#!/usr/bin/env python3
"""Compile the patched GPIO bodies against deterministic libusb transfer mocks.

This exercises the exact source fragment from the GPL fork. No RTL device,
libusb installation, or Heimdall binary is used.
"""
import argparse
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_SOURCE_ROOT = ROOT / 'build/kraken-macos/upstream/librtlsdr'


def fragment(source, start, end):
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=pathlib.Path, default=DEFAULT_SOURCE_ROOT,
                        help='librtlsdr source root containing src/librtlsdr.c')
    args = parser.parse_args()
    source_file = args.source.resolve() / 'src/librtlsdr.c'
    if not source_file.is_file():
        parser.error(f'missing librtlsdr source: {source_file}')
    source = source_file.read_text()
    gpio = fragment(source, '/* GPIO control must retain libusb failures', 'int rtlsdr_get_gpio_bit')
    bias = fragment(source, 'int rtlsdr_set_bias_tee_gpio', 'int rtlsdr_set_bias_tee(')
    harness = f'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define SYSB 2
#define GPD 0x3001
#define GPO 0x3003
#define GPOE 0x3004
#define SOFTSTATE_RESET 3
typedef struct rtlsdr_dev {{ uint32_t gpio_state_known, gpio_state; }} rtlsdr_dev_t;
static int outcomes[8], at, reads, writes, agc;
static void plan(const int *values, int count) {{ int i; at=reads=writes=agc=0; for(i=0;i<count;i++) outcomes[i]=values[i]; }}
int rtlsdr_read_array(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *value, uint8_t length) {{ int r=outcomes[at++]; reads++; if(r==1) *value=0xa5; return r; }}
int rtlsdr_write_array(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *value, uint8_t length) {{ writes++; return outcomes[at++]; }}
static int reactivate_softagc(rtlsdr_dev_t *dev, int state) {{ agc++; return 0; }}
{gpio}
{bias}
static void require(int condition, const char *message) {{ if (!condition) {{ fprintf(stderr, "%s\\n", message); exit(1); }} }}
int main(void) {{
  rtlsdr_dev_t dev={{0,0}}; int ok[]={{1,1,1,1,1,1}}; int values[6]; int phase, kind;
  plan(ok,6); require(rtlsdr_set_bias_tee_gpio(&dev,2,1)==0,"success ABI"); require(agc==1,"AGC only after success"); require(dev.gpio_state_known==(1U<<2),"state cache only after output success");
  for (kind=0; kind<2; kind++) for (phase=0; phase<4; phase++) {{
    int i; for(i=0;i<6;i++) values[i]=1; values[phase]=kind ? -(40+phase) : 0;
    dev.gpio_state_known=dev.gpio_state=0; plan(values,6);
    require(rtlsdr_set_bias_tee_gpio(&dev,2,1)==(kind ? values[phase] : -1),"output transfer propagated");
    require(agc==0 && dev.gpio_state_known==0,"output failure cannot update cache or AGC");
  }}
  for (kind=0; kind<2; kind++) for (phase=0; phase<2; phase++) {{
    int i; for(i=0;i<6;i++) values[i]=1; values[phase]=kind ? -(60+phase) : 0;
    dev.gpio_state_known=(1U<<2); dev.gpio_state=0; plan(values,6);
    require(rtlsdr_set_bias_tee_gpio(&dev,2,1)==(kind ? values[phase] : -1),"bit transfer propagated");
    require(agc==0,"bit failure cannot activate AGC");
  }}
  require(rtlsdr_set_bias_tee_gpio(&dev,-1,1)==-1 && rtlsdr_set_bias_tee_gpio(&dev,8,1)==-1,"invalid gpio");
  return 0;
}}
'''
    with tempfile.TemporaryDirectory(prefix='vectorwarp-kraken-gpio-') as directory:
        directory = pathlib.Path(directory)
        program = directory / 'gpio.c'
        binary = directory / 'gpio'
        program.write_text(harness)
        subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Werror', str(program), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('Kraken RTL-SDR GPIO/bias-tee USB error propagation PASS (mock transfers).')


if __name__ == '__main__':
    main()
