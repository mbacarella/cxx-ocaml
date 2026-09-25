module type O = sig type t end
module type H = sig module E : O type h end
module M : H with type E.t = int = struct module E = struct type t = int end type h = unit end
let y : M.E.t = 3
