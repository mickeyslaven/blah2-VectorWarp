#!/usr/bin/env ruby
# Export the exact, platform-evaluated inputs of frozen installed Homebrew recipes.
require "digest"
require "fileutils"
require "json"
require "open-uri"
require "optparse"
require "pathname"

options = {}
OptionParser.new do |parser|
  %i[specs api_metadata output manifest arch source_id run_id].each do |name|
    parser.on("--#{name.to_s.tr('_', '-')} VALUE") { |value| options[name] = value }
  end
end.parse!
abort "missing arguments" unless options.values_at(:specs, :api_metadata, :output, :manifest, :arch, :source_id, :run_id).all?
abort "invalid architecture" unless %w[arm64 x86_64].include?(options[:arch])
abort "invalid source identity" unless options[:source_id].match?(/\A[0-9a-f]{40}\z/) && options[:run_id].match?(/\A\d+\z/)

root = Pathname(options[:output]).expand_path
root.mkpath
api_metadata = JSON.parse(Pathname(options[:api_metadata]).read)

def file_sha(path)
  Digest::SHA256.file(path).hexdigest
end

def safe(value)
  abort "unsafe formula input identity" unless value.match?(/\A[A-Za-z0-9@+_.-]+\z/)
  value
end

def immutable_revision(downloadable)
  specs = downloadable.respond_to?(:specs) ? downloadable.specs : {}
  revision = specs[:revision] || specs["revision"]
  return revision if revision.to_s.match?(/\A[0-9a-f]{40}\z/)
  url = downloadable.respond_to?(:url) ? downloadable.url.to_s : ""
  match = url.match(%r{/(?:commit|raw)/([0-9a-f]{40})(?:[/.?]|\z)})
  match && match[1]
end

def urls(downloadable)
  values = [downloadable.url.to_s]
  values.concat(downloadable.mirrors.map(&:to_s)) if downloadable.respond_to?(:mirrors)
  values.reject(&:empty?).uniq
end

def materialize(downloadable, destination, formula, version, input_id)
  downloadable.fetch
  cached = Pathname(downloadable.cached_download).expand_path
  abort "Homebrew input was not fetched: #{formula} #{input_id}" unless cached.exist?
  checksum = downloadable.respond_to?(:checksum) && downloadable.checksum ? downloadable.checksum.hexdigest : nil
  revision = immutable_revision(downloadable)
  segment = safe(input_id.gsub(":", "-"))
  directory = destination / "source-cache" / safe(formula) / safe(version) / segment
  directory.mkpath
  if cached.directory?
    abort "mutable VCS input: #{formula} #{input_id}" unless revision
    actual = `git -C #{cached.to_s.shellescape} rev-parse HEAD`.strip
    abort "VCS revision differs: #{formula} #{input_id}" unless $?.success? && actual == revision
    system("git", "-C", cached.to_s, "cat-file", "-e", "#{revision}:.gitmodules", out: File::NULL, err: File::NULL)
    abort "VCS submodules require explicit closure support: #{formula} #{input_id}" if $?.success?
    target = directory / "source.tar"
    abort "git archive failed" unless system("git", "-C", cached.to_s, "archive", "--format=tar", "--output", target.to_s, revision)
    kind = "vcs"
  else
    abort "special Homebrew input" unless cached.file?
    target = directory / (cached.basename.to_s.empty? ? "source" : cached.basename.to_s)
    FileUtils.copy_file(cached, target)
    abort "Homebrew checksum differs: #{formula} #{input_id}" if checksum && file_sha(target) != checksum
    abort "mutable unchecked input: #{formula} #{input_id}" unless checksum || revision
    kind = input_id.start_with?("patch:") ? "external-patch" : "archive"
  end
  {"id" => input_id, "kind" => kind, "urls" => urls(downloadable),
   "checksum" => checksum && {"algorithm" => "sha256", "value" => checksum},
   "revision" => revision,
   "payload" => {"path" => target.relative_path_from(destination).to_s,
                 "sha256" => file_sha(target), "size" => target.size}}
end

def materialize_local_patch(patch, metadata, destination, formula, version, input_id)
  path = patch.file.to_s
  checksum = metadata.fetch("local_patch_checksums", {}).fetch(path, nil)
  revision = metadata["tap_git_head"]
  tap = metadata["tap"]
  abort "unbound local patch: #{formula} #{path}" unless checksum&.match?(/\A[0-9a-f]{64}\z/) && revision&.match?(/\A[0-9a-f]{40}\z/) && tap == "homebrew/core"
  abort "unsafe local patch path" if path.start_with?("/") || Pathname(path).each_filename.any? { |part| part == ".." }
  url = "https://raw.githubusercontent.com/Homebrew/homebrew-core/#{revision}/#{path}"
  directory = destination / "source-cache" / safe(formula) / safe(version) / safe(input_id.gsub(":", "-"))
  directory.mkpath; target = directory / Pathname(path).basename
  URI.open(url, "rb", open_timeout: 30, read_timeout: 120) { |source| target.open("wb") { |sink| IO.copy_stream(source, sink) } }
  abort "local patch checksum differs" unless file_sha(target) == checksum
  {"id" => input_id, "kind" => "local-patch", "urls" => [url],
   "checksum" => {"algorithm" => "sha256", "value" => checksum}, "revision" => revision,
   "payload" => {"path" => target.relative_path_from(destination).to_s,
                 "sha256" => checksum, "size" => target.size}}
end

require "shellwords"
rows = Pathname(options[:specs]).read.lines.filter_map do |line|
  next if line.strip.empty?
  formula, version, usages, recipe = line.chomp.split("\t", 4)
  abort "invalid frozen formula specification" unless recipe
  [safe(formula), safe(version), usages.split(",").sort.uniq, Pathname(recipe).expand_path]
end
abort "empty frozen formula specification" if rows.empty?

seen = {}
formulas = rows.sort_by { |row| row[0, 2] }.map do |formula, version, usages, recipe|
  identity = "#{formula}|#{version}"
  abort "duplicate frozen formula identity" if seen[identity]
  seen[identity] = true
  before = file_sha(recipe)
  metadata = api_metadata.fetch(formula, {})
  api_recipe = metadata.dig("ruby_source_checksum", "sha256")
  abort "Homebrew API recipe differs from installed recipe: #{formula}" if api_recipe && api_recipe != before
  loaded = Formulary.factory(recipe)
  abort "frozen recipe identity differs: expected #{formula}|#{version}, got #{loaded.name}|#{loaded.pkg_version}" \
    unless loaded.name == formula && loaded.pkg_version.to_s == version
  recipe_copy = root / "recipes" / formula / version / "formula.rb"
  recipe_copy.dirname.mkpath
  FileUtils.copy_file(recipe, recipe_copy)
  inputs = [materialize(loaded.stable, root, formula, version, "primary")]
  loaded.resources.sort_by(&:name).each do |resource|
    inputs << materialize(resource, root, formula, version, "resource:#{safe(resource.name)}")
  end
  loaded.patchlist.each_with_index do |patch, index|
    if patch.class.name == "DATAPatch"
      value = patch.contents.to_s
      inputs << {"id" => "patch:#{index}", "kind" => "embedded-patch",
                 "strip" => patch.strip.to_s, "embedded_sha256" => Digest::SHA256.hexdigest(value)}
    elsif patch.class.name == "LocalPatch"
      inputs << materialize_local_patch(patch, metadata, root, formula, version, "patch:#{index}")
    elsif patch.respond_to?(:fetch)
      inputs << materialize(patch, root, formula, version, "patch:#{index}")
    else
      abort "unsupported patch type: #{patch.class}"
    end
  end
  abort "frozen recipe changed during export" unless file_sha(recipe) == before && file_sha(recipe_copy) == before
  {"id" => identity, "formula" => formula, "version" => version, "usage" => usages,
   "recipe" => {"path" => recipe_copy.relative_path_from(root).to_s, "sha256" => before, "size" => recipe_copy.size},
   "dependencies" => loaded.deps.map(&:name).sort.uniq, "inputs" => inputs}
end

manifest = {"schema" => 2, "arch" => options[:arch], "source_id" => options[:source_id],
            "run_id" => options[:run_id], "brew_version" => HOMEBREW_VERSION,
            "formulas" => formulas}
Pathname(options[:manifest]).write(JSON.pretty_generate(manifest) + "\n")
