module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module F (X : sig type t = int end) = struct end
