module M : sig end = struct module F (X : sig type t val v : int end) =
  struct type u = X.t end end
