module X = struct module type S = sig type t val x : int module M : sig type s
  end end end
type t = (module X.S with type t = unit)
