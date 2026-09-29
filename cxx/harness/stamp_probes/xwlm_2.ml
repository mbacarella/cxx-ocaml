module type O = sig type t end
module type H = sig module E : O type h end
module F (X : H with type E.t = int) = struct let f (x : X.E.t) = x + 1 end
