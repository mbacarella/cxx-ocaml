module F (X : sig module N : sig val x : int val y : int end end) = struct
  let f () = X.N.x end
