# All engine schemas compile into one module, `schemas`, included as <schemas/<stem>.h>.
engine_schema_library(NAME schemas
  SCHEMAS
    provenance.schema)
