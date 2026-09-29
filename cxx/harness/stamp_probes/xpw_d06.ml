module type S = sig type t module M : sig type s val v : int module N : sig
  type w end end end
type u = (module S with type t = int)
