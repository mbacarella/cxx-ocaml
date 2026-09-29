(* in a signature and a submodule *)
module M : sig type t = E of int [@@unboxed] end = struct
  type t = E of int [@@unboxed]
end
module N = struct type t = { x : unit } [@@unboxed] end
