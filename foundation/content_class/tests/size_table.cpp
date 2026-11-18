// Size table for foundation/content_class (ADR-0019). A class is kept per mesh of a loaded scene
// and joined into every derived node, and a policy rides in a scene's description, so both stay
// a few bytes and byte-aligned: an unclassified asset costs sixteen zero bytes and one test.
#include <core/base/size_table.h>
#include <foundation/content_class/content_class.h>

using namespace engine;

// One level byte per category, k_max_categories of them.
ENGINE_EXPECT_SIZE(16, 1, content_class::ContentClasses);

// A cap byte and a substitution byte per category.
ENGINE_EXPECT_SIZE(32, 1, content_class::ViewPolicy);

// The outcome and the deciding category, level and cap.
ENGINE_EXPECT_SIZE(4, 1, content_class::Decision);
