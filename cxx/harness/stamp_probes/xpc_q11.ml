module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class virtual b = object method virtual a : int M.s -> int end
class d = object inherit b method a = fun x -> 0 end
