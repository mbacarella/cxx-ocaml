type r = {f : 'a. 'a -> 'a; g : int}
module M = struct let v {f; g} = f g let w x = x.f 3 end
