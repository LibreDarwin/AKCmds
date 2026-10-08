/* Fixed bytes of the ODF (odt) members that do not vary with the
 * document, and the pieces of those that do, taken from the
 * reference tool verbatim.  The font-face and P1 pieces are written
 * only when the document holds a paragraph; the empty document
 * leaves both out.  Generated from /usr/bin/textutil probes; do not
 * edit by hand. */

static const char O_HEAD[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" xmlns:number=\"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" xmlns:chart=\"urn:oasis:names:tc:opendocument:xmlns:chart:1.0\" xmlns:dr3d=\"urn:oasis:names:tc:opendocument:xmlns:dr3d:1.0\" xmlns:math=\"http://www.w3.org/1998/Math/MathML\" xmlns:form=\"urn:oasis:names:tc:opendocument:xmlns:form:1.0\" xmlns:script=\"urn:oasis:names:tc:opendocument:xmlns:script:1.0\" xmlns:ooo=\"http://openoffice.org/2004/office\" xmlns:ooow=\"http://openoffice.org/2004/writer\" xmlns:oooc=\"http://openoffice.org/2004/calc\" xmlns:dom=\"http://www.w3.org/2001/xml-events\" xmlns:xforms=\"http://www.w3.org/2002/xforms\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" office:version=\"1.0\"><office:font-face-decls>";

static const char O_FONTFACE[] =
"<style:font-face style:name=\"Helvetica Light\" svg:font-family=\"Helvetica Light\"/>";

static const char O_ENDFONT[] =
"</office:font-face-decls>";

static const char O_AUTO[] =
"<office:automatic-styles>";

static const char O_P1[] =
"<style:style style:name=\"P1\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties><style:tab-stops><style:tab-stop style:position=\"0.3889in\"/><style:tab-stop style:position=\"0.7778in\"/><style:tab-stop style:position=\"1.1667in\"/><style:tab-stop style:position=\"1.5556in\"/><style:tab-stop style:position=\"1.9444in\"/><style:tab-stop style:position=\"2.3333in\"/><style:tab-stop style:position=\"2.7222in\"/><style:tab-stop style:position=\"3.1111in\"/><style:tab-stop style:position=\"3.5000in\"/><style:tab-stop style:position=\"3.8889in\"/><style:tab-stop style:position=\"4.2778in\"/><style:tab-stop style:position=\"4.6667in\"/></style:tab-stops></style:paragraph-properties><style:text-properties style:font-name=\"Helvetica Light\" fo:font-size=\"12.0pt\"/></style:style>";

static const char O_TC[] =
"<style:style style:name=\"TableColumn1\" style:family=\"table-column\"><style:table-column-properties style:column-width=\"3.4625in\" style:rel-column-width=\"32767*\"/></style:style>";

static const char O_ENDSTYLE[] =
"</office:automatic-styles>";

static const char O_MID[] =
"<office:body><office:text>";

static const char O_TAIL[] =
"</office:text></office:body></office:document-content>";

static const char OMETA_HEAD[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" xmlns:ooo=\"http://openoffice.org/2004/office\" office:version=\"1.0\"><office:meta><meta:generator>CocoaODFWriter/2685.6</meta:generator>";

static const char OMETA_TAIL[] =
"</office:meta></office:document-meta>";

static const char STYLES[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" xmlns:number=\"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" xmlns:chart=\"urn:oasis:names:tc:opendocument:xmlns:chart:1.0\" xmlns:dr3d=\"urn:oasis:names:tc:opendocument:xmlns:dr3d:1.0\" xmlns:math=\"http://www.w3.org/1998/Math/MathML\" xmlns:form=\"urn:oasis:names:tc:opendocument:xmlns:form:1.0\" xmlns:script=\"urn:oasis:names:tc:opendocument:xmlns:script:1.0\" xmlns:ooo=\"http://openoffice.org/2004/office\" xmlns:ooow=\"http://openoffice.org/2004/writer\" xmlns:oooc=\"http://openoffice.org/2004/calc\" xmlns:dom=\"http://www.w3.org/2001/xml-events\" office:version=\"1.0\"><office:font-face-decls><style:font-face style:name=\"Times\" svg:font-family=\"Times\"/></office:font-face-decls><office:styles><style:default-style style:family=\"paragraph\"><style:paragraph-properties style:tab-stop-distance=\"0.5in\"/><style:text-properties style:font-name=\"Times\" fo:font-size=\"12.0pt\"/></style:default-style><style:default-style style:family=\"table\"><style:table-properties table:border-model=\"collapsing\"/></style:default-style><style:default-style style:family=\"table-row\"><style:table-row-properties fo:keep-together=\"auto\"/></style:default-style><style:style style:name=\"Standard\" style:family=\"paragraph\" style:class=\"text\"/></office:styles><office:automatic-styles><style:page-layout style:name=\"Standard\"><style:page-layout-properties fo:page-width=\"8.5in\" fo:page-height=\"11.0in\" style:print-orientation=\"portrait\" fo:margin-top=\"0.5in\" fo:margin-bottom=\"0.5in\" fo:margin-left=\"1.0in\" fo:margin-right=\"1.0in\"/></style:page-layout></office:automatic-styles><office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"Standard\"></style:master-page></office:master-styles></office:document-styles>";

static const char MANIFEST[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE manifest:manifest PUBLIC \"-//OpenOffice.org//DTD Manifest 1.0//EN\" \"Manifest.dtd\">\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\">\n <manifest:file-entry manifest:media-type=\"application/vnd.oasis.opendocument.text\" manifest:full-path=\"/\"/>\n <manifest:file-entry manifest:media-type=\"text/xml\" manifest:full-path=\"content.xml\"/>\n <manifest:file-entry manifest:media-type=\"text/xml\" manifest:full-path=\"styles.xml\"/>\n <manifest:file-entry manifest:media-type=\"text/xml\" manifest:full-path=\"meta.xml\"/>\n</manifest:manifest>";

static const char MIMETYPE[] =
"application/vnd.oasis.opendocument.text";

