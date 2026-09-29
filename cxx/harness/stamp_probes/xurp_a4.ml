(* control: float and untagged reprs are immediates *)
external f : float -> float = "f" "f_u" [@@unboxed]
external g : (int [@untagged]) -> int = "g" "g_u"
