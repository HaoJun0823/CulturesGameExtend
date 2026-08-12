#include <windows.h>
#include <stdio.h>
#include <string.h>
int main(){
    HDC dc=CreateCompatibleDC(NULL);
    int heightPx=48;
    HFONT f=CreateFontW(-heightPx,0,0,0,400,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft JhengHei");
    SelectObject(dc,f);
    TEXTMETRICW tm={}; GetTextMetricsW(dc,&tm);
    int ascent=tm.tmAscent;
    int cellW=heightPx*2+6, cellH=heightPx+20, baseY=ascent+5;
    printf("ascent=%d tmHeight=%d tmDescent=%d tmIntLead=%d cellW=%d cellH=%d baseY=%d\n",
           ascent,tm.tmHeight,tm.tmDescent,tm.tmInternalLeading,cellW,cellH,baseY);
    BITMAPINFOHEADER bi={}; bi.biSize=sizeof(bi); bi.biWidth=cellW; bi.biHeight=-cellH; bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB;
    void* bits=0; HBITMAP bmp=CreateDIBSection(dc,(BITMAPINFO*)&bi,DIB_RGB_COLORS,&bits,0,0);
    if(!bmp){printf("no dib err=%lu\n",GetLastError());return 1;}
    SelectObject(dc,bmp);
    SetBkMode(dc,OPAQUE); SetBkColor(dc,RGB(255,255,255)); SetTextColor(dc,RGB(0,0,0));
    memset(bits,0xFF,(size_t)cellW*cellH*4);
    const wchar_t wc=0x7E41; // 繁
    BOOL r=TextOutW(dc,3,baseY,&wc,1);
    printf("TextOut ret=%d err=%lu\n",r,GetLastError());
    unsigned char* p=(unsigned char*)bits; int nonwhite=0,minx=cellW,miny=cellH,maxx=-1,maxy=-1;
    for(int y=0;y<cellH;y++)for(int x=0;x<cellW;x++){unsigned char*px=p+(y*cellW+x)*4; if(!(px[0]==255&&px[1]==255&&px[2]==255)){nonwhite++;if(x<minx)minx=x;if(y<miny)miny=y;if(x>maxx)maxx=x;if(y>maxy)maxy=y;}}
    printf("繁: nonwhite=%d bbox=(%d,%d)-(%d,%d)\n",nonwhite,minx,miny,maxx,maxy);
    // also ASCII 'A'
    memset(bits,0xFF,(size_t)cellW*cellH*4);
    r=TextOutW(dc,3,baseY,L"A",1);
    printf("TextOut A ret=%d err=%lu\n",r,GetLastError());
    nonwhite=0;minx=cellW;miny=cellH;maxx=-1;maxy=-1;
    for(int y=0;y<cellH;y++)for(int x=0;x<cellW;x++){unsigned char*px=p+(y*cellW+x)*4; if(!(px[0]==255&&px[1]==255&&px[2]==255)){nonwhite++;if(x<minx)minx=x;if(y<miny)miny=y;if(x>maxx)maxx=x;if(y>maxy)maxy=y;}}
    printf("A: nonwhite=%d bbox=(%d,%d)-(%d,%d)\n",nonwhite,minx,miny,maxx,maxy);
    return 0;
}
