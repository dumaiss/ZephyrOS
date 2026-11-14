"""Decoder-only lifecycle links no terminal screen model."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class DecoderOnlyTests(unittest.TestCase):
    def test_keys_lifecycle_and_no_cell_arrays(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            (temp/'kos.h').write_text('#include "kos/types.h"\n#include "kos/errno.h"\nvoid idle(void);\n')
            (temp/'probe.c').write_text(r'''
#include "tui.h"
#include "kos/errno.h"
#include <assert.h>
#include <string.h>
static const char *input="";
int console_read(char *out,unsigned int n) { (void)n;if(!*input)return 0;*out=*input++;return 1; }
void idle(void) {}
int main(void) {
 assert(tui_key_pending()==-KOS_EINVAL && tui_pollkey()==TUI_KEY_NONE);
 assert(tui_key_init()==0 && tui_key_init()==-KOS_EINVAL);
 input="a\033[D\033";
 assert(tui_key_pending()==1 && tui_pollkey()=='a');
 assert(tui_getkey()==TUI_KEY_LEFT && tui_pollkey()==TUI_KEY_ESC);
 assert(tui_pollkey()==TUI_KEY_NONE);
 input="\033[?";
 assert(tui_pollkey()==TUI_KEY_ESC && tui_pollkey()=='[' && tui_pollkey()=='?');
 tui_key_shutdown();assert(tui_pollkey()==TUI_KEY_NONE && tui_key_pending()==-KOS_EINVAL);
 assert(tui_key_init()==0); input="z"; assert(tui_getkey()=='z');tui_key_shutdown();
 return 0;
}
''')
            exe = temp/'keys'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                            '-I'+str(temp),'-I'+str(ROOT/'user/sdk/kres/host'),
                            '-I'+str(ROOT/'abi'),'-I'+str(ROOT/'user/sdk/include'),
                            str(temp/'probe.c'),str(ROOT/'user/sdk/tui/src/tui_key.c'),
                            str(ROOT/'user/sdk/tui/src/tui_core.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
            symbols = subprocess.check_output(['nm',str(exe)],text=True)
            self.assertNotIn(' tui_chars',symbols)
            self.assertNotIn(' tui_attrs',symbols)
