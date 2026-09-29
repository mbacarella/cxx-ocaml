(* two externals with no C stub name share primitive.ml's one "" *)
external f : int -> int = "caml_probe_f"
external g : int -> int = "caml_probe_g"
let h x = f (g x)
