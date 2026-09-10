# blah2Fast

A CPU multithreaded and GPU Accelerated real-time radar which can support various SDR platforms. This is a faster processing fork of the original blah2 program by 30hours. 

## Features

- All features from normal blah2 plus the following additions:
  - Full krakenSDR Support
  - Full UI rewrite with a new clean modern look
  - Web configuration and radar restart right from the browser
  - Easy recording and replay. Simply select the file and play back that once in a lifetime radar catch again and again!

## SDR Support

- [SDRplay RSPDuo](https://www.sdrplay.com/rspduo/).
- [USRP](https://www.ettus.com/products/) (only tested on the B210).
- 2x [HackRF](https://greatscottgadgets.com/hackrf/) with clock synchronisation and hardware trigger.
- 2x [RTL-SDR](https://www.rtl-sdr.com/) with clock synchronisation.
- [KrakenSDR](https://www.krakenrf.com/) with 2-8x channels using the kraken V2 software (8 Channel support cant be tested, as their 8 channel device isn't available).

## Services

The build environment consists of a docker-compose.yml file running the following services;

- The radar processor responsible for IQ capture and processing.
- The API middleware responsible for reading TCP ports for delay-Doppler map data, and exposing this on a REST API.
- The web front-end displaying processed radar data.

## Usage

Building the code using the following instructions; 

- Install docker and docker-compose on the host machine.
- Clone this repository to some directory.
- Install SDRplay API to run service on host.
- Edit the `config/config.yml` for desired processing parameters.
- Run the docker-compose command.

```bash
sudo git clone http://github.com/mickeyslaven/blah2Fast /opt/blah2
cd /opt/blah2
sudo chown -R $USER .
sudo chmod a+x ./lib/sdrplay-3.15.2/SDRplay_RSP_API-Linux-3.15.2.run
sudo ./lib/sdrplay-3.15.2/SDRplay_RSP_API-Linux-3.15.2.run --tar -xvf -C ./lib/sdrplay-3.15.2
cd lib/sdrplay-3.15.2/ && sudo ./install_lib.sh && cd ../../
sudo docker network create blah2
sudo systemctl enable docker
sudo docker compose up -d --build
```

The radar processing output is available on [http://localhost:49152](http://localhost:49152).

## Future Work

- Utilizing the krakenSDR for bearing to be able to plot planes onto a map. Alternatively you can use 3lips which utilizes elipses and multiple reciever locations [3lips](https://github.com/30hours/3lips).
- 

## FAQ

- If the SDRplay RSPduo does not capture data, restart the API service (on the host) using the script `sudo ./script/blah2_rspduo_restart.bash`.

## Contributing

Pull requests are welcome :)

## Links

- Join the [Discord](https://discord.gg/ewNQbeK5Zn) chat for sharing results and support. Keep in mind this discord is for blah2 and not everyone will be familiar with this specific fork.

## License

[MIT](https://choosealicense.com/licenses/mit/)
