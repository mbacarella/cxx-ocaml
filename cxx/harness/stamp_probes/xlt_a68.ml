module Id (X : sig type t end) = X
module A = struct type t end
let f () = let module M = struct type u = Id (A).t end in ()
