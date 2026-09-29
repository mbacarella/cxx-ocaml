module Id (X : sig type t end) = X
module type S = sig val s : unit end
module G (X : sig type t = int val x : t module M : S end) = struct
  type t = X.t
  let y = X.x
  module Y = X
  let _ = Y.x
  let () = X.M.s
  type u = Id (X).t
end
