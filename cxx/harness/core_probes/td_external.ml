external id : 'a -> 'a = "%identity"
external f : int -> int = "caml_f" [@@noalloc]
external g : (float [@unboxed]) -> (float [@unboxed]) = "g_byte" "g_nat"
external u : int -> (int [@untagged]) = "u_b" "u_n"
