'use strict';

// Fixed guidance only: no browser value becomes a shell argument or installer.
function receiverSetupGuide(receiver, helperExecutable) {
  const steps = [];
  if (!receiver.capabilities.liveCompiled) {
    steps.push({
    text: 'Install a VectorWarp build containing this live receiver backend. Installing its driver alone cannot add a missing backend.',
    link: 'https://github.com/mickeyslaven/blah2-VectorWarp/blob/main/docs/INSTALL.md',
    label: 'Receiver build and installation guide'});
    return steps;
  }
  if (receiver.type === 'RspDuo' && receiver.dependencies.state !== 'installed') {
    steps.push({text: receiver.dependencies.state === 'missing' ?
      'SDRplay API was not found. Obtain it from SDRplay and accept its license locally before enrolling an installed service.' :
      'Could not verify SDRplay API. Check its local installation before enrolling the service.',
    link: 'https://sdrplay.com/hardware-api/', label: 'SDRplay hardware API and supported systems'});
  }
  if (receiver.type === 'Kraken' && receiver.locality === 'remote') {
    steps.push({text: 'Keep the existing remote Suite installation. Enter its saved host and IQ/control ports, then Apply to verify the complete receiver settings. Local service management does not control that host.'});
  } else if ((receiver.type === 'Kraken' ||
      (receiver.type === 'RspDuo' && receiver.dependencies.state === 'installed')) &&
      receiver.managedService.state !== 'running') {
    steps.push({text: 'An administrator can enroll an already-installed local receiver service once. Review its exact installed definition in the terminal. Then Check receiver software here and review its start action.',
      command: `sudo ${helperExecutable} enroll-service ${receiver.type}`});
  }
  if (receiver.type === 'HackRF' && receiver.dependencies.state !== 'installed') {
    steps.push({text: 'On qualified Fedora 44 or Ubuntu/Debian (including an Ubuntu-based DragonOS with native Ubuntu repositories), enroll a complete signed native package transaction in the administrator terminal. Then return here to review every package and run the separately authorized one-use install.',
      command: `sudo ${helperExecutable} enroll-packages HackRF`});
    steps.push({text: 'Use the signed native distribution package in your local package manager. Review repository origins and the complete proposed changes before approving installation. On Ubuntu/Debian the package is hackrf; on Fedora it is hackrf. Return here and recheck after installation.',
      link: 'https://hackrf.readthedocs.io/en/latest/installing_hackrf_software.html', label: 'Official HackRF installation guide'});
  }
  if (receiver.type === 'Usrp' && receiver.dependencies.state !== 'installed') {
    steps.push({text: 'On qualified Fedora 44 or Ubuntu/Debian providing UHD 4.8 or newer, enroll the complete signed native package transaction locally. Then review its full package list and authorize installation here.',
      command: `sudo ${helperExecutable} enroll-packages Usrp`});
    steps.push({text: 'Install UHD 4.8 or newer using the signed native distribution repository if that version is available. Review the proposed transaction locally. If the distribution is older, follow the UHD source-build guide; do not replace system packages with an unreviewed repository.',
      link: 'https://github.com/EttusResearch/uhd/blob/master/host/docs/install.dox', label: 'Official UHD installation guide'});
  }
  steps.push({text: 'Save for later keeps a draft on disk. Apply confirms the supported receiver settings and requests the configured processor restart. Then inspect fresh processor status; software discovery alone does not verify RF, wiring or calibration.'});
  return steps;
}
module.exports = {receiverSetupGuide};
