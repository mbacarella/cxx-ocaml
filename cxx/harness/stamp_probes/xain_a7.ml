(* include of a named module type of *)
module type T = module type of Option
include (Option : T)
