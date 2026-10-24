// Linux CPU-only compiler check. Never creates a Vulkan instance/device.
#include "../../core/VulkanBackend.private.cpp"
namespace {
void compile(const std::string& name,const std::string& source){
 glslang_input_t input{};input.language=GLSLANG_SOURCE_GLSL;input.stage=GLSLANG_STAGE_COMPUTE;
 input.client=GLSLANG_CLIENT_VULKAN;input.client_version=GLSLANG_TARGET_VULKAN_1_0;
 input.target_language=GLSLANG_TARGET_SPV;input.target_language_version=GLSLANG_TARGET_SPV_1_0;
 input.code=source.c_str();input.default_version=450;input.default_profile=GLSLANG_NO_PROFILE;
 input.messages=static_cast<glslang_messages_t>(GLSLANG_MSG_SPV_RULES_BIT|GLSLANG_MSG_VULKAN_RULES_BIT);
 input.resource=glslang_default_resource();
 std::unique_ptr<glslang_shader_t,decltype(&glslang_shader_delete)> shader(glslang_shader_create(&input),glslang_shader_delete);
 if(!shader||!glslang_shader_preprocess(shader.get(),&input)||!glslang_shader_parse(shader.get(),&input))
  throw std::runtime_error(name+": "+(shader?glslang_shader_get_info_log(shader.get()):"allocation failed"));
 std::unique_ptr<glslang_program_t,decltype(&glslang_program_delete)> program(glslang_program_create(),glslang_program_delete);
 if(!program)throw std::runtime_error("Program allocation failed");
 glslang_program_add_shader(program.get(),shader.get());
 if(!glslang_program_link(program.get(),input.messages))throw std::runtime_error(name+": "+glslang_program_get_info_log(program.get()));
 glslang_program_SPIRV_generate(program.get(),input.stage);
 if(!glslang_program_SPIRV_get_size(program.get()))throw std::runtime_error("SPIR-V generation failed");
}
}
int main(){try{
 if(!glslang_initialize_process())throw std::runtime_error("Shader compiler unavailable");
 unsigned count=0;
 for(bool replay:{false,true}){
  for(const auto& shader:blah2::gpu_postprocess::shaderSources(replay)){compile(shader.first,shader.second);++count;}
  for(bool coherent:{false,true}){const auto source=blah2::gpu_resident::fusionShader(coherent,replay);
   if(!replay&&source!=(coherent?blah2::gpu_resident::coherentFusionSource:blah2::gpu_resident::fusionSource))
    throw std::runtime_error("Default-off fusion source changed");
   compile("fusion",source);++count;}
 }
 glslang_finalize_process();std::cout<<"COMPLETE replay shader compile count="<<count<<" gpu_execution=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL replay shaders: "<<e.what()<<'\n';return 1;}}
