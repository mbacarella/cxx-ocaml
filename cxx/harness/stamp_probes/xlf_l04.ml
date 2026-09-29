module M : sig end = struct module type T =
  sig module G : sig module H : sig type t end end end end
