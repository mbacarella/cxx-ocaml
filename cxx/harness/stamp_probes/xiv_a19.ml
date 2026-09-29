class virtual a = object val virtual x : int end
class b = object inherit a inherit a val x = 1 end
