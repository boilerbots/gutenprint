/*
 * "$Id$"
 *
 *   printdef XML parser - process gimp-print XML data with libxml2.
 *
 *   Copyright 1997-2000 Michael Sweet (mike@easysw.com),
 *	Robert Krawitz (rlk@alum.mit.edu) and Michael Natterer (mitch@gimp.org)
 *   Copyright 2002 Roger Leigh (roger@whinlatter.uklinux.net)
 *
 *   This program is free software; you can redistribute it and/or modify it
 *   under the terms of the GNU General Public License as published by the Free
 *   Software Foundation; either version 2 of the License, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *   for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <libxml/parser.h>
#include <gimp-print/gimp-print.h>
#include "../main/papers.h"
#include "../main/util.h"
#include "../main/vars.h"


void printer_output_start(void);
void paper_output_start(void);
void output_paper(stp_internal_papersize_t *paper);
void printer_output_end(void);
void paper_output_end(void);
int xmlstrtol(xmlChar *value);
int xmlstrtoul(xmlChar *value);
float xmlstrtof(xmlChar *textval);
void stp_xml_process_gimpprint(xmlNodePtr gimpprint, int mode);
void stp_xml_process_printdef(xmlNodePtr printdef);
void stp_xml_process_family(xmlNodePtr family);
void stp_xml_process_printer(xmlNodePtr printer, xmlChar *family);
void stp_xml_process_paperdef(xmlNodePtr paperdef);
stp_internal_papersize_t *stp_xml_process_paper(xmlNodePtr paper);
xmlNodePtr stp_xml_get_node(xmlNodePtr prop, const xmlChar *name);

/* Available "family" drivers */
const char *family_names[] =
{
  "canon",
  "escp2",
  "pcl",
  "ps",
  "lexmark",
  "raw",
  NULL
};

enum
  {
    XMLDEF_PRINTER,
    XMLDEF_PAPER
  };

int main(int argc, char *argv[])
{
  xmlDocPtr doc;
  xmlNodePtr cur;
  int mode;

  if (argc != 3)
    {
      fprintf(stderr, "Usage: %s (papers|printers) file.xml\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  if (!strcmp(argv[1], "printers"))
    mode = XMLDEF_PRINTER;
  else if (!strcmp(argv[1], "papers"))
    mode = XMLDEF_PAPER;
  else
    {
      fprintf(stderr, "Invalid mode `%s'\n", argv[1]);
      exit (EXIT_FAILURE);
    }

#ifdef DEBUG
  fprintf(stderr, "Reading XML file `%s'...", argv[2]);
#endif

  doc = xmlParseFile(argv[2]);

  if (doc == NULL )
    {
      fprintf(stderr,"XML file not parsed successfully. \n");
      xmlFreeDoc(doc);
      return EXIT_FAILURE;
    }

  cur = xmlDocGetRootElement(doc);

  if (cur == NULL)
    {
      fprintf(stderr,"empty document\n");
      xmlFreeDoc(doc);
      return EXIT_FAILURE;
    }

  if (xmlStrcmp(cur->name, (const xmlChar *) "gimp-print"))
    {
      fprintf(stderr,"XML file of the wrong type, root node != gimp-print");
      xmlFreeDoc(doc);
      return EXIT_FAILURE;
    }

  /* The XML file was read and is the right format */

#ifdef DEBUG
  fprintf(stderr, "done.\n");
  fprintf(stderr, "Writing header...");
#endif
  if (mode == XMLDEF_PRINTER)
    printer_output_start();
  else if (mode == XMLDEF_PAPER)
    paper_output_start();
#ifdef DEBUG
  fprintf(stderr, "done.\n");

  fprintf(stderr, "Processing XML parse tree:\n");
#endif
  stp_xml_process_gimpprint(cur, mode);

#ifdef DEBUG
  fprintf(stderr, "Writing footer...");
#endif
  if (mode == XMLDEF_PRINTER)
    printer_output_end();
  else if (mode == XMLDEF_PAPER)
    paper_output_end();
#ifdef DEBUG
  fprintf(stderr, "done.\n");
#endif

  return EXIT_SUCCESS;
}

void printer_output_start(void)
{
  int i = 0;

  fputs("/* This file is automatically generated.  See printers.xml.\n"
	"   DO NOT EDIT! */\n\n",
	stdout);
  while(family_names[i])
    {
      fprintf(stdout, "const extern stp_printfuncs_t stp_%s_printfuncs;\n",
	      (const char *) family_names[i]);
      i++;
    }
  fputs("\nstatic const stp_old_printer_t stp_old_printer_list[] =\n"
	"{\n",
	stdout);
}

void paper_output_start(void)
{
  fputs("/* This file is automatically generated.  See papers.xml.\n"
	"   DO NOT EDIT! */\n\n",
	stdout);
  fputs("\nstatic stp_internal_papersize_t paper_sizes[] =\n"
	"{\n",
	stdout);
}

void output_paper(stp_internal_papersize_t *paper)
{
  if (paper)
    {
      fprintf(stdout, "  {\n");
      fprintf(stdout, "    \"%s\",\n", paper->name);
      fprintf(stdout, "    N_ (\"%s\"),\n", paper->text);
      fprintf(stdout, "    %u,\n", paper->width);
      fprintf(stdout, "    %u,\n", paper->height);
      fprintf(stdout, "    %u,\n", paper->top);
      fprintf(stdout, "    %u,\n", paper->left);
      fprintf(stdout, "    %u,\n", paper->bottom);
      fprintf(stdout, "    %u,\n", paper->right);
      if (paper->paper_unit == PAPERSIZE_ENGLISH)
	fprintf(stdout, "    PAPERSIZE_ENGLISH\n");
      else if (paper->paper_unit == PAPERSIZE_METRIC)
	fprintf(stdout, "    PAPERSIZE_METRIC\n");
      fprintf(stdout, "  },\n");
    }
}

void printer_output_end(void)
{
  const char *footer =
    "};\n"
    "/* End of generated data */\n";

  fputs(footer, stdout);
}

void paper_output_end(void)
{
  const char *footer =
    "  {\n    \"\",\n    \"\",\n    0,\n    0,\n    0,\n    0,\n    0,\n    PAPERSIZE_METRIC\n  }\n"
    "};\n"
    "/* End of generated data */\n";

  fputs(footer, stdout);
}

int
xmlstrtol(xmlChar* textval)
 {
  int val;
  val = strtol((const char *) textval, (char **)NULL, 10);

  if (val == LONG_MIN || val == LONG_MAX)
    {
      fprintf(stderr, "Value incorrect: %s\n",
	      strerror(errno));
      exit (EXIT_FAILURE);
    }
  return val;
}

int
xmlstrtoul(xmlChar* textval)
 {
  int val;
  val = strtoul((const char *) textval, (char **)NULL, 10);

  if (val == ULONG_MAX)
    {
      fprintf(stderr, "Value incorrect: %s\n",
	      strerror(errno));
      exit (EXIT_FAILURE);
    }
  return val;
}


float
xmlstrtof(xmlChar *textval)
{
  float val;
  val = strtod((const char *) textval, (char **)NULL);

  if (val == HUGE_VAL || val == -HUGE_VAL)
    {
      fprintf(stderr, "Value incorrect: %s\n",
	      strerror(errno));
      exit (EXIT_FAILURE);
    }
  return (float) val;
}


void
stp_xml_process_gimpprint(xmlNodePtr cur, int mode)
{
  xmlNodePtr child;
  child = cur->children;
  while (child)
    {
      if (mode == XMLDEF_PRINTER)
	{
	  if (!xmlStrcmp(child->name, (const xmlChar *) "printdef"))
	    stp_xml_process_printdef(child);
	}
      else if (mode == XMLDEF_PAPER)
	{
	  if (!xmlStrcmp(child->name, (const xmlChar *) "paperdef"))
	    stp_xml_process_paperdef(child);
	}
      child = child->next;
    }
}


void
stp_xml_process_printdef(xmlNodePtr printdef)
{
  xmlNodePtr family;

  family = printdef->children;
  while (family)
    {
      if (!xmlStrcmp(family->name, (const xmlChar *) "family"))
	{
	  stp_xml_process_family(family);
	}
      family = family->next;
    }
}


void
stp_xml_process_family(xmlNodePtr family)
{
  xmlChar *family_name;
  xmlNodePtr printer;
  int i = 0, family_valid = 0;

  family_name = xmlGetProp(family, (const xmlChar *) "name");
  while (family_names[i])
    {
      if (!xmlStrcmp(family_name, (const xmlChar *) family_names[i]))
	  family_valid = 1;
      i++;
    }
#ifdef DEBUG
  fprintf(stderr, "  %s:\n", (const char *) family_name);
#endif

  printer = family->children;
  while (family_valid && printer)
    {
      if (!xmlStrcmp(printer->name, (const xmlChar *) "printer"))
	stp_xml_process_printer(printer, family_name);
      printer = printer->next;
    }
}

const char *keylist[] =
{
  "Cyan",
  "Magenta",
  "Yellow",
  "Brightness",
  "Contrast",
  "Gamma",
  "Density",
  "Saturation",
  NULL
};

xmlNodePtr
stp_xml_get_node(xmlNodePtr prop, const xmlChar *name)
{
  while (prop)
    {
      if (!xmlStrcmp(prop->name, name))
	return prop;
      prop = prop->next;
    }
  return (xmlNodePtr) NULL;
}

void
stp_xml_process_printer(xmlNodePtr printer, xmlChar *family)
{
  xmlNodePtr prop;

  printf("  {\n");
  printf("    \"%s\",\n",
	 (const char *) xmlGetProp(printer, (const xmlChar *) "name"));
  printf("    \"%s\",\n",
	 (const char *) xmlGetProp(printer, (const xmlChar *) "driver"));
  printf("    \"%s\",\n", (const char *) family);
  prop = stp_xml_get_node(printer->children, (const xmlChar *) "model");
  printf("    %d,\n",
	 xmlstrtol(xmlGetProp(prop, (const xmlChar *) "value")));
  prop = stp_xml_get_node(printer->children, (const xmlChar *) "color");
  printf("    %d,\n",
	 xmlStrcmp(xmlGetProp(prop, (const xmlChar *) "value"),
		   (const xmlChar *) "true") == 0 ?
	 OUTPUT_COLOR : OUTPUT_GRAY);
  printf("    &stp_%s_printfuncs,\n", family);

  prop = printer->children;
  while(prop)
    {
      const char **key = &(keylist[0]);
      while (*key)
	{
	  if (strcasecmp((const char *) (prop->name), *key) == 0)
	    {
	      float val =
		xmlstrtof(xmlGetProp(prop, (const xmlChar *) "value"));
	      if (val > 0 && val != 1.0)
		printf("    \"%s=%f;\"\n", *key, val);
	      break;
	    }
	  key++;
	}
      prop = prop->next;
    }
  printf("  },\n");
}


void
stp_xml_process_paperdef(xmlNodePtr printdef)
{
  xmlNodePtr paper;

  paper = printdef->children;
  while (paper)
    {
      if (!xmlStrcmp(paper->name, (const xmlChar *) "paper"))
	{
	  output_paper(stp_xml_process_paper(paper));
	}
      paper = paper->next;
    }
}

stp_internal_papersize_t *
stp_xml_process_paper(xmlNodePtr paper)
{
  xmlNodePtr prop;
  /* props[] (unused) is the correct tag sequence */
  /*  const char *props[] =
    {
      "name",
      "description",
      "width",
      "height",
      "unit",
      NULL
      };*/
  static stp_internal_papersize_t outpaper;
  /* Check tags are present */
  int
    id = 0,
    name = 0,
    height = 0,
    width = 0,
    unit = 0;
  outpaper.name =
    (const char *) xmlGetProp(paper, (const xmlChar *) "name");
  if (outpaper.name)
    id = 1;

  prop = paper->children;
  while(prop)
    {
      if (!xmlStrcmp(prop->name, (const xmlChar *) "description"))
	{
	  outpaper.text = (const char *)
	    xmlGetProp(prop, (const xmlChar *) "value");
	  name = 1;
	}
      if (!xmlStrcmp(prop->name, (const xmlChar *) "comment"))
	{
	  /* Ignore for now */
	}
      if (!xmlStrcmp(prop->name, (const xmlChar *) "width"))
	{
	  outpaper.width =
	    xmlstrtoul(xmlGetProp(prop,
				  (const xmlChar *) "value"));
	  width = 1;
	}
      if (!xmlStrcmp(prop->name, (const xmlChar *) "height"))
	{
	  outpaper.height =
	    xmlstrtoul(xmlGetProp(prop,
				  (const xmlChar *) "value"));
	  height = 1;
	}
      if (!xmlStrcmp(prop->name, (const xmlChar *) "unit"))
	{
	  if (!xmlStrcmp(xmlGetProp(prop,
				    (const xmlChar *) "value"),
			 (const xmlChar *) "english"))
	    outpaper.paper_unit = PAPERSIZE_ENGLISH;
	  else if (!xmlStrcmp(xmlGetProp(prop,
					 (const xmlChar *) "value"),
			      (const xmlChar *) "metric"))
	    outpaper.paper_unit = PAPERSIZE_METRIC;
	  /* Default unit? */
	  unit = 1;
	}

      outpaper.top = 0;
      outpaper.left = 0;
      outpaper.bottom = 0;
      outpaper.right = 0;

      prop = prop->next;
    }
  if (id && name && width && height && unit)
    return &outpaper;
  return NULL;
}
