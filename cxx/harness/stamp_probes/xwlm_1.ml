module type O = sig type t end
module type P = sig module E : O end
module type H = sig module Q : P end
module type MK = H with type Q.E.t = int
