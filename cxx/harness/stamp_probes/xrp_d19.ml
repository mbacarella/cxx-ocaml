module type A = sig type t = private < m : int; .. > end
module F (X : A) = struct include X let y = 1 end
module M = F (struct type t = < m : int > end)
