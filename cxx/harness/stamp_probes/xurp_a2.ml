(* the three unboxed integer kinds, each shared with itself *)
external f : int32 -> int64 -> nativeint = "f" "f_u" [@@unboxed]
external g : nativeint -> int32 -> int64 = "g" "g_u" [@@unboxed]
