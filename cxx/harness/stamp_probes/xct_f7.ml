module G (X : sig end) = struct type 'a t = 'a list end;;
module M1 = struct end;;
module type S = sig val f : int G(M1).t -> int end;;
