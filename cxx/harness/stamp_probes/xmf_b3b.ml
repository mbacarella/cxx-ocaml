module type O = sig type t val eq : t -> t -> bool end
module type H = sig module Elem : O type heap val empty : heap end
module L (E : O) : H with module Elem = E = struct module Elem = E
  type heap = int let empty = 0 end
module I = struct type t = int let eq = (=) end
module B (MakeH : functor (E : O) -> H) (E : O) : H = struct module Elem = E
  type heap = int let empty = 0 end
module C = B(L)(I)
