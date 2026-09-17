module type S = sig type t module M : sig type s module N : sig type w end end
  end
type u = (module S with type t = int)
