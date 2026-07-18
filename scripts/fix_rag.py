import os
os.chdir(r'C:/Users/sulaimanalshammari/Casper_Engine')
c = open('Core_CPP/casper_rag.c','r',encoding='utf-8').read()
fixes = [
    ('WINHTTP_ADD_REQ_FLAG_ADD', 'WINHTTP_ADDREQ_FLAG_ADD'),
]
for old,new in fixes:
    c = c.replace(old, new)
# fix the empty path L"" -> wp
import re
c = re.sub(r'L"GET",\(wchar_t\*\)L""', 'L"GET",wp', c)
open('Core_CPP/casper_rag.c','w',encoding='utf-8').write(c)
print('done')
