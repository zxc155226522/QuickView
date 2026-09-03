#include <cstdio>
#include "librevenge/librevenge.h"
#include "librevenge-generators/librevenge-generators.h"

int main()
{
  librevenge::RVNGStringVector out;
  librevenge::RVNGSVGDrawingGenerator gen(out, "");
  librevenge::RVNGPropertyList pl;
  pl.insert("svg:width", 4.0, librevenge::RVNG_INCH);
  pl.insert("svg:height", 2.0, librevenge::RVNG_INCH);
  gen.startDocument(pl);
  librevenge::RVNGPropertyList pageProps;
  gen.startPage(pageProps);
  librevenge::RVNGPropertyList tp;
  tp.insert("svg:x", 1.0, librevenge::RVNG_INCH);
  tp.insert("svg:y", 1.0, librevenge::RVNG_INCH);
  tp.insert("svg:width", 2.0, librevenge::RVNG_INCH);
  tp.insert("svg:height", 0.3, librevenge::RVNG_INCH);
  gen.startTextObject(tp);
  librevenge::RVNGPropertyList pp;
  gen.openParagraph(pp);
  librevenge::RVNGPropertyList sp;
  sp.insert("fo:font-size", 12.0, librevenge::RVNG_POINT);
  gen.openSpan(sp);
  librevenge::RVNGString text("Keyboard");
  printf("text len=%zu empty=%d\n", text.len(), text.empty() ? 1 : 0);
  gen.insertText(text);
  gen.closeSpan();
  gen.closeParagraph();
  gen.endTextObject();
  gen.endPage();
  gen.endDocument();
  printf("pages=%zu\n", out.size());
  if (out.size())
    printf("svg: %s\n", out[0].cstr());
  return 0;
}
