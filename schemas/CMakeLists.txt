# All engine schemas compile into one module, `schemas`, included as <schemas/<stem>.h>.
engine_schema_library(NAME schemas
  SCHEMAS
    provenance.schema
    doc.schema
    gfx.schema
    scene.schema
    protocol.schema
    tissue.schema)
