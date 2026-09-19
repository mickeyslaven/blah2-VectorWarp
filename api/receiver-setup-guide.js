'use strict';

// Fixed guidance only: no browser value becomes a shell argument or installer.
function receiverSetupGuide(receiver, helperExecutable, {platform = process.platform} = {}) {
  if (platform === 'darwin') return macosReceiverSetupGuide(receiver);
  const steps = [];
  if (!receiver.capabilities.liveCompiled) {
    steps.push({
    text: 'Install a VectorWarp build with support for this receiver. Installing a driver alone will not add support.',
    link: 'https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/INSTALL.md',
    label: 'Receiver build and installation guide'});
    return steps;
  }
  if (receiver.type === 'RspDuo' && receiver.dependencies.state !== 'installed') {
    steps.push({text: receiver.dependencies.state === 'missing' ?
      'SDRplay API was not found. Install it from SDRplay, then return and choose Save & Restart.' :
      'Could not verify SDRplay API. Check its local installation; VectorWarp does not download it or accept its license.',
    link: 'https://sdrplay.com/hardware-api/', label: 'SDRplay hardware API and supported systems'});
  }
  if (receiver.type === 'Kraken' && receiver.locality === 'remote') {
    steps.push({text: 'Use the existing remote Suite. Enter its host and IQ/control ports, then Apply. Local service controls cannot manage that host.'});
  } else if (receiver.type === 'RspDuo' && receiver.dependencies.state === 'installed' &&
      receiver.managedService.state !== 'running') {
    steps.push({text: 'Save & Restart starts the installed standard SDRplay API service, or reuses it if running. An administrator must review custom or overridden services.',
      command: `sudo ${helperExecutable} enroll-service RspDuo`});
  } else if (receiver.type === 'Kraken' &&
      receiver.managedService.state !== 'running') {
    steps.push({text: 'An administrator can enroll an installed local receiver service once. Review its installed definition in the terminal, then check receiver software and its start action.',
      command: `sudo ${helperExecutable} enroll-service ${receiver.type}`});
  }
  if (receiver.type === 'HackRF' && receiver.dependencies.state !== 'installed') {
    steps.push({text: 'On supported Fedora 44 or Ubuntu/Debian, including DragonOS with native Ubuntu repositories, enroll the signed package transaction in the administrator terminal. Then review the packages here and run the one-use install.',
      command: `sudo ${helperExecutable} enroll-packages HackRF`});
    steps.push({text: 'Use the signed distribution package in your local package manager. Review repository origins and proposed changes before installing. The package is hackrf on Ubuntu/Debian and Fedora. Then recheck here.',
      link: 'https://hackrf.readthedocs.io/en/latest/installing_hackrf_software.html', label: 'Official HackRF installation guide'});
  }
  if (receiver.type === 'Usrp' && receiver.dependencies.state !== 'installed') {
    steps.push({text: 'On supported Fedora 44 or Ubuntu/Debian with UHD 4.1 or newer, enroll the signed package transaction locally. Then review the package list and authorize installation here.',
      command: `sudo ${helperExecutable} enroll-packages Usrp`});
    steps.push({text: 'Install UHD 4.1 or newer from the signed distribution repository when available. Review the proposed transaction. For older distributions, use the UHD source-build guide; do not replace system packages from an unreviewed repository.',
      link: 'https://github.com/EttusResearch/uhd/blob/master/host/docs/install.dox', label: 'Official UHD installation guide'});
  }
  steps.push({text: 'Save for later keeps a draft. Apply confirms supported settings and requests a processor restart. Then check processor status; software discovery does not verify RF, wiring, or calibration.'});
  return steps;
}
function macosReceiverSetupGuide(receiver) {
  const steps = [];
  const standalone = receiver.capabilities?.standaloneDistribution === true;
  if (standalone && (receiver.type === 'Usrp' || receiver.type === 'HackRF') &&
      receiver.capabilities.liveCompiled && receiver.capabilities.runtimeLoadable === false)
    steps.push({text: 'The bundled receiver adapter could not load. Reinstall the standalone VectorWarp package, then recheck receiver software. Do not use Homebrew to repair this bundle.'});
  if (receiver.type === 'RspDuo') {
    steps.push({text: 'Obtain the macOS SDRplay API and headers directly from SDRplay and install them yourself. VectorWarp never downloads, bundles, or accepts the license for this SDK. Manage its API service using the vendor instructions.',
      link: 'https://sdrplay.com/hardware-api/', label: 'SDRplay hardware API'});
  } else if (receiver.type === 'Kraken' && standalone) {
    steps.push({text: 'Local Kraken capture requires the separately installed local Heimdall companion. Install or repair that companion, then recheck receiver software. This standalone package does not offer a remote-Suite fallback.'});
  } else if (receiver.type === 'Kraken') {
    steps.push({text: 'Run KrakenSDR Suite V2 on its supported Linux receiver host, then enter that host and its IQ/control ports here. VectorWarp on macOS uses the network stream and does not manage the remote service.'});
  } else if (receiver.type === 'HackRF' && standalone) {
    steps.push({text: receiver.capabilities.runtimeLoadable === true ?
      'The bundled HackRF adapter is loadable. Live dual-HackRF use still requires two physical devices and separate hardware verification.' :
      'The bundled HackRF adapter is unavailable. Reinstall the standalone VectorWarp package, then recheck receiver software. Do not install Homebrew libraries to repair this bundle.'});
  } else if (receiver.type === 'HackRF') {
    steps.push({text: 'Install libhackrf using the official macOS instructions, then build VectorWarp with its HackRF adapter. Dual HackRF operation requires two devices and separate hardware verification.',
      link: 'https://hackrf.readthedocs.io/en/latest/installing_hackrf_software.html', label: 'Official HackRF installation guide'});
  } else if (receiver.type === 'Usrp' && standalone) {
    steps.push({text: receiver.capabilities.runtimeLoadable === true ?
      'The bundled UHD adapter is loadable. Install required UHD device images manually under your VectorWarp state directory at uhd-images, or set UHD_IMAGES_DIR. Device images and hardware readiness are separate.' :
      'The bundled UHD adapter is unavailable. Reinstall the standalone VectorWarp package, then recheck receiver software. Install UHD device images manually only after the bundled adapter is loadable.'});
  } else if (receiver.type === 'Usrp') {
    steps.push({text: 'Install UHD 4.1 or newer and the appropriate device images using Ettus Research’s macOS instructions, then build the USRP adapter. Library detection does not verify a B210 or USB streaming.',
      link: 'https://files.ettus.com/manual/page_install.html', label: 'Official UHD installation guide'});
  }
  if (!receiver.capabilities.liveCompiled) steps.push({text: standalone && receiver.type === 'RspDuo' ?
    'Build the local RSPduo adapter after installing the vendor SDK and compatible Apple command-line tools. This adapter is deliberately omitted from the standalone package; reinstalling the package will not add it.' : standalone ?
    'This standalone package supports replay for this profile but lacks its live adapter. Reinstall the standalone package and recheck receiver software.' :
    'This build supports replay for this profile but does not contain its live adapter. Rebuild with the locally installed SDK to enable live capture.',
    link: 'https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/MACOS.md', label: 'macOS source build guide'});
  steps.push({text: 'Save & Restart applies the saved processor configuration. Receiver services remain independently managed. Check fresh processor status; software discovery and replay do not verify physical hardware.'});
  return steps;
}
module.exports = {receiverSetupGuide};
