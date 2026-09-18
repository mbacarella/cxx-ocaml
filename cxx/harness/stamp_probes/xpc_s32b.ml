module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
let f () = let module N = struct module P : sig type t = int end = struct
  type t = int end end in 0
