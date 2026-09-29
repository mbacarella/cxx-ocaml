(* in a submodule and at top level *)
module M = struct
  external f : int64 -> int64 = "f" "f_u" [@@unboxed]
end
external g : int64 -> int64 = "g" "g_u" [@@unboxed]
