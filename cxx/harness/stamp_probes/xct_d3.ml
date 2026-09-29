module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
module G (X : sig end) = struct
  class type ['a] t = object method m : 'a end end;;
class type u = object inherit [int] G(M1).t end;;
