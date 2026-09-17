module type S = sig type 'a w end
module F (X : S) = struct type u = int X.w end
