'use strict';

// Fixed guidance only: no browser value becomes a shell argument or installer.
function receiverSetupGuide(receiver, helperExecutable) {
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
module.exports = {receiverSetupGuide};
