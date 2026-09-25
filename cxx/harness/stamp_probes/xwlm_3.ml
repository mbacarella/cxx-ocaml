module type O = sig type t end
module type H = sig module E : O type h end
module type S = sig module M : H with type E.t = bool val v : M.E.t end
