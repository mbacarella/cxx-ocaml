(* a signature-ascribed module *)
module M : sig
  external f : int64 -> int64 = "f" "f_u" [@@unboxed]
end = struct
  external f : int64 -> int64 = "f" "f_u" [@@unboxed]
end
