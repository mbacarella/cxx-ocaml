module F (X : sig type t end) = struct
  type u = A of X.t | B type r = { x : X.t } end
module M = F (Int)
module N = F (String)
