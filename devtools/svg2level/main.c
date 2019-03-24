#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlstring.h>
#include <libxml/debugXML.h>

#define RECTS_CAPACITY 1024
#define TEXTS_CAPACITY 1024
#define BUFFER_CAPACITY 1024

#ifdef LIBXML_TREE_ENABLED

typedef struct {
    xmlNode **rects;
    size_t rects_count;
    xmlNode **texts;
    size_t texts_count;
    xmlNode **buffer;
} Context;

static void print_usage(FILE *stream)
{
    fprintf(stream, "Usage: ./svg2level <input.svg> <output.txt>");
}

static void fail_node(xmlNode *node, const char *format, ...)
{
    va_list args;
    va_start(args, format);

    vfprintf(stderr, format, args);
    xmlDebugDumpNode(stderr, node, 0);

    va_end(args);
    exit(-1);
}

static size_t extract_nodes_by_name(xmlNode *root, const char *node_name, xmlNode **rects, size_t n)
{
    (void) node_name;

    if (n == 0) {
        return 0;
    }

    const size_t old_n = n;

    for (xmlNode *iter = root; n > 0 && iter; iter = iter->next) {
        if (iter->type == XML_ELEMENT_NODE && xmlStrEqual(iter->name, (const xmlChar*)node_name)) {
            rects[0] = iter;
            n--;
            rects++;

        }

        const size_t m = extract_nodes_by_name(iter->children, node_name, rects, n);

        n -= m;
        rects += m;
    }

    return old_n - n;
}

static const char *color_of_style(const char *style)
{
    const char *prefix = "fill:#";
    const char *fill = strstr(style, prefix);
    if (fill == NULL) {
        return NULL;
    }
    return fill + strlen(prefix);
}

static xmlNode *find_attr_by_name(xmlNode *node, const char *attr_name)
{
    for (xmlAttr *attr = node->properties; attr; attr = attr->next) {
        if (xmlStrEqual(attr->name, (const xmlChar *)attr_name)) {
            return attr->children;
        }
    }

    return NULL;
}

static xmlNode *require_attr_by_name(xmlNode *node, const char *attr_name)
{
    xmlNode *attr = find_attr_by_name(node, attr_name);
    if (attr == NULL) {
        fail_node(node, "Required attribute `%s`\n", attr_name);
    }
    return attr;
}

static const char *require_color_of_node(xmlNode *node)
{
    xmlNode *style_attr = require_attr_by_name(node, "style");
    const char *color = color_of_style((const char*)style_attr->content);
    if (color == NULL) {
        fail_node(node, "`style` attr does not define the `fill` of the rectangle\n");
    }
    return color;
}

static size_t filter_nodes_by_id_prefix(xmlNode **input, size_t input_count,
                                        const char* id_prefix,
                                        xmlNode **output, size_t output_capacity)
{
    size_t output_count = 0;

    for (size_t i = 0; i < input_count && output_count < output_capacity; ++i) {
        xmlNode *id_attr = require_attr_by_name(input[i], "id");
        if (xmlStrstr(id_attr->content, (const xmlChar*)id_prefix) == id_attr->content) {
            output[output_count++] = input[i];
        }
    }

    return output_count;
}

static xmlNode *find_node_by_id(xmlNode **nodes, size_t n, const char *id)
{
    for (size_t i = 0; i < n; ++i) {
        xmlNode *idAttr = find_attr_by_name(nodes[i], "id");
        if (idAttr != NULL && xmlStrEqual(idAttr->content, (const xmlChar*)id)) {
            return nodes[i];
        }
    }

    return NULL;
}

static xmlNode *require_node_by_id(xmlNode **nodes, size_t n, const char *id)
{
    xmlNode *node = find_node_by_id(nodes, n, id);
    if (node == NULL) {
        fail_node(node, "Could find node with id `%s`\n", id);
    }
    return node;
}

static void save_pack_by_id_prefix(Context *context, FILE *output_file,
                                   const char *id_prefix,
                                   const char **attrs, size_t attrs_count)
{
    xmlNode **pack = context->buffer;
    size_t pack_count = filter_nodes_by_id_prefix(
        context->rects, context->rects_count, id_prefix,
        pack, BUFFER_CAPACITY);
    pack_count += filter_nodes_by_id_prefix(
        context->texts, context->texts_count, id_prefix,
        pack + pack_count, BUFFER_CAPACITY - pack_count);

    fprintf(output_file, "%ld\n", pack_count);
    for (size_t i = 0; i < pack_count; ++i) {
        for (size_t j = 0; j < attrs_count; ++j) {
            if (strcmp(attrs[j], "#color") == 0) {
                const char *color = require_color_of_node(pack[i]);
                fprintf(output_file, "%.6s ", color);
            } else {
                xmlNode *attr = require_attr_by_name(pack[i], attrs[j]);
                fprintf(output_file, "%s ", attr->content);
            }
        }
        fprintf(output_file, "\n");
    }
}

////////////////////////////////////////////////////////////

static void save_title(Context *context, FILE *output_file)
{
    xmlNode *node = require_node_by_id(context->texts, context->texts_count, "title");
    for (xmlNode *iter = node->children; iter; iter = iter->next) {
        fprintf(output_file, "%s", iter->children->content);
    }
    fprintf(output_file, "\n");
}

static void save_background(Context *context, FILE *output_file)
{
    fprintf(output_file, "%.6s\n",
            require_color_of_node(
                require_node_by_id(
                    context->rects, context->rects_count,
                    "background")));
}

static void save_player(Context *context, FILE *output_file)
{
    xmlNode *node = require_node_by_id(context->rects, context->rects_count, "player");
    const char *color = require_color_of_node(node);
    xmlNode *xAttr = require_attr_by_name(node, "x");
    xmlNode *yAttr = require_attr_by_name(node, "y");
    fprintf(output_file, "%s %s %.6s\n", xAttr->content, yAttr->content, color);
    // TODO(#751): save_player does not support the player's script
}

static void save_platforms(Context *context, FILE *output_file)
{
    const char *attrs[] = {"x", "y", "width", "height", "#color"};
    const size_t attrs_count = sizeof(attrs) / sizeof(const char*);
    save_pack_by_id_prefix(context, output_file, "rect", attrs, attrs_count);
}

static void save_goals(Context *context, FILE *output_file)
{
    const char *attrs[] = {"id", "x", "y", "#color"};
    const size_t attrs_count = sizeof(attrs) / sizeof(const char*);
    save_pack_by_id_prefix(context, output_file, "goal", attrs, attrs_count);
}

static void save_lavas(Context *context, FILE *output_file)
{
    const char *attrs[] = {"x", "y", "width", "height", "#color"};
    const size_t attrs_count = sizeof(attrs) / sizeof(const char*);
    save_pack_by_id_prefix(context, output_file, "lava", attrs, attrs_count);
}

static void save_backplatforms(Context *context, FILE *output_file)
{
    const char *attrs[] = {"x", "y", "width", "height", "#color"};
    const size_t attrs_count = sizeof(attrs) / sizeof(const char*);
    save_pack_by_id_prefix(context, output_file, "backrect", attrs, attrs_count);
}

static void save_boxes(Context *context, FILE *output_file)
{
    // TODO(#741): save_boxes is not implemented
    (void) context;
    (void) output_file;
}

static void save_labels(Context *context, FILE *output_file)
{
    // TODO(#742): save_labels is not implemented
    (void) context;
    (void) output_file;
}

static void save_script_regions(Context *context, FILE *output_file)
{
    // TODO(#743): save_script_regions is not implemented
    (void) context;
    (void) output_file;
}

static void save_level(Context *context, FILE *output_file)
{
    save_title(context, output_file);
    save_background(context, output_file);
    save_player(context, output_file);
    save_platforms(context, output_file);
    save_goals(context, output_file);
    save_lavas(context, output_file);
    save_backplatforms(context, output_file);
    save_boxes(context, output_file);
    save_labels(context, output_file);
    save_script_regions(context, output_file);
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        print_usage(stderr);
        return -1;
    }

    LIBXML_TEST_VERSION

    const char *input_filename = argv[1];
    const char *output_filename = argv[2];

    xmlDoc *doc = xmlReadFile(input_filename, NULL, 0);
    if (doc == NULL) {
        fprintf(stderr, "Could not parse file `%s`\n", input_filename);
        return -1;
    }

    xmlNode *root = xmlDocGetRootElement(doc);

    FILE *output_file = fopen(output_filename, "w");
    if (output_file == NULL) {
        fprintf(stderr, "Could not open file `%s`\n", output_filename);
        return -1;
    }

    Context context;
    context.rects = calloc(RECTS_CAPACITY, sizeof(xmlNode*));
    context.rects_count = extract_nodes_by_name(root, "rect", context.rects, RECTS_CAPACITY);
    context.texts = calloc(TEXTS_CAPACITY, sizeof(xmlNode*));
    context.texts_count = extract_nodes_by_name(root, "text", context.texts, TEXTS_CAPACITY);
    context.buffer = calloc(BUFFER_CAPACITY, sizeof(xmlNode*));

    save_level(&context, output_file);

    fclose(output_file);
    xmlFreeDoc(doc);
    xmlCleanupParser();

    return 0;
}

#else
int main(void) {
    fprintf(stderr, "Tree support not compiled in\n");
    exit(1);
}
#endif