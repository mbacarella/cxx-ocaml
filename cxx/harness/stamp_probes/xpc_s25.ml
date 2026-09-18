module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module type T = sig type t = int M.s end
module N : sig include T end = struct type t = int M.s end
