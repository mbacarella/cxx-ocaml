(* a result and an argument share the block *)
external f : int64 -> float -> int64 = "f" "f_u" [@@unboxed]
