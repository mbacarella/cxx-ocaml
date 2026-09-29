module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
module G (X : sig end) = struct
  class type ['a] t = object method m : 'a end end;;
let f (x : int G(M1).t) = x;;
