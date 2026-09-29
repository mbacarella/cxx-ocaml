(* a WRITTEN native name is the declaration's own string *)
external f : float -> float = "caml_probe_bf" "caml_probe_nf"
external g : float -> float = "caml_probe_bg" "caml_probe_ng"
external h : int -> int = "caml_probe_h"
