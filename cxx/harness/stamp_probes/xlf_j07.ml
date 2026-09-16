module M : sig end = struct module N = struct module P =
  struct module type T = sig type t end end end end
