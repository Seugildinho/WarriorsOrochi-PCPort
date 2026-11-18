"""Send one opt-in, auto-releasing pad command to the local diagnostic build.

This controls only the prototype's test pad service; it never injects OS keys.
STOP asks the runtime to return normally, allowing an explicitly enabled RAM dump.
"""
import argparse
import time
from run_probe import PORT, BUTTONS

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('button',type=str.upper,choices=[*BUTTONS,'STOP'])
    parser.add_argument('--frames',type=int,default=30)
    parser.add_argument('--stick',type=int,nargs=2,metavar=('LX','LY'),help='Left stick 0..255 (128 = centre) while held')
    args=parser.parse_args()
    if not 1<=args.frames<=120:parser.error('Hold duration must be 1..120 UI frames')
    target=PORT/'local/probe-control.txt'
    if not target.exists():parser.error('Start run_probe.py with --live-inputs first')
    command='STOP' if args.button=='STOP' else str(BUTTONS[args.button])
    temporary=target.with_suffix('.tmp')
    stick=f' {args.stick[0]} {args.stick[1]}' if args.stick else ''
    temporary.write_text(f'{time.monotonic_ns()} {command} {args.frames}{stick}\n',encoding='ascii')
    for attempt in range(20):
        try:
            temporary.replace(target)
            break
        except PermissionError:
            if attempt==19:raise
            time.sleep(0.025)
    print(f'Internal test command: {args.button}, duration={args.frames} UI frames')

if __name__=='__main__':main()
