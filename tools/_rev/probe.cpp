#pragma region Glyph render probe
#include <windows.h>
#include <stdio.h>
int main(){
    HDC dc=CreateCompatibleDC(NULL);
    if(!dc){printf("no dc\n");return 1;}
    int W=64,H=64;
    BITMAPINFOHEADER bi={}; bi.biSize=sizeof(bi); bi.biWidth=W; bi.biHeight=-H; bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB;
    void* bits=0; HBITMAP bmp=CreateDIBSection(dc,(BITMAPINFO*)&bi,DIB_RGB_COLORS,&bits,0,0);
    if(!bmp){printf("no dib\n");return 1;}
    SelectObject(dc,bmp);
    HFONT f=CreateFontW(-32,0,0,0,400,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft JhengHei");
    SelectObject(dc,f);
    SetBkMode(dc,OPAQUE); SetBkColor(dc,RGB(255,255,255)); SetTextColor(dc,RGB(0,0,0));
    memset(bits,0xFF,W*H*4);
    BOOL r=TextOutW(dc,4,40,L"A",1); printf("TextOut A ret=%d err=%lu\n",r,GetLastError());
    unsigned char* p=(unsigned char*)bits; int nonwhite=0,minx=W,miny=H,maxx=-1,maxy=-1;
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){unsigned char*px=p+(y*W+x)*4; if(!(px[0]==255&&px[1]==255&&px[2]==255)){nonwhite++;if(x<minx)minx=x;if(y<miny)miny=y;if(x>maxx)maxx=x;if(y>maxy)maxy=y;}}
    printf("A: nonwhite=%d bbox=(%d,%d)-(%d,%d)\n",nonwhite,minx,miny,maxx,maxy);
    memset(bits,0xFF,W*H*4);
    r=TextOutW(dc,4,40,L"\x7e41",1); printf("TextOut U+7E41 ret=%d err=%lu\n",r,GetLastError());
    nonwhite=0;minx=W;miny=H;maxx=-1;maxy=-1;
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){unsigned char*px=p+(y*W+x)*4; if(!(px[0]==255&&px[1]==255&&px[2]==255)){nonwhite++;if(x<minx)minx=x;if(y<miny)miny=y;if(x>maxx)maxx=x;if(y>maxy)maxy=y;}}
    printf("U+7E41: nonwhite=%d bbox=(%d,%d)-(%d,%d)\n",nonwhite,minx,miny,maxx,maxy);
    return 0;
}

#pragma endregion
