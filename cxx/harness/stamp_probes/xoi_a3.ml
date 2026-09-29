type 'a t = ..
type 'a u = 'a t = ..
module M = struct type ('a, 'b) s = .. end
