module F (X : sig module N : sig val x : int val y : int end end) = struct
  module T = X let f () = T.N.x end
