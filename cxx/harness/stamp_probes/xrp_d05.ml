module type A = sig type t = private < m : int; .. > end
module F (X : A) = struct type t = X.t let y = 1 end
