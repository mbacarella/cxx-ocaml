(* known miss: a name from the enclosing signature gives up *)
module type P = sig
  type 'a t
  module I : sig type nonrec 'a t = 'a t end
end
