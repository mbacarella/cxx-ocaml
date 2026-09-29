(* two int64 externals cite one Unboxed_integer Pint64 *)
external f : int64 -> int64 = "f" "f_u" [@@unboxed]
external g : (int64 [@unboxed]) -> int = "g" "g_u"
