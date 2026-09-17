module M = struct class virtual a = object method virtual m : int end end
class virtual b = object inherit M.a end
