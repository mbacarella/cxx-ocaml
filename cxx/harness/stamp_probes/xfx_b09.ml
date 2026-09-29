module type S = sig type 'a w end
module F (X : S) = struct type u = A of int X.w end
