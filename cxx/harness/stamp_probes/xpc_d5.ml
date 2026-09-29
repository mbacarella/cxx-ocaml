module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
type t = int M.s
